import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Data;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class ExtractMain extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        Memory mem = currentProgram.getMemory();

        StringBuilder json = new StringBuilder();
        json.append("{\n");
        json.append("  \"binary_name\": \"").append(currentProgram.getName()).append("\",\n");
        json.append("  \"architecture\": \"").append(currentProgram.getLanguage().getProcessor().toString()).append("\",\n");
        json.append("  \"functions\": [\n");

        FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
        boolean firstFunc = true;

        while (functions.hasNext() && !monitor.isCancelled()) {
            Function func = functions.next();
            if (!func.getName().toLowerCase().contains("main")) {
                continue;
            }

            if (!firstFunc) json.append(",\n");
            firstFunc = false;

            // 1. Descompilação C
            DecompileResults res = decompiler.decompileFunction(func, 60, monitor);
            String cCode = (res != null && res.decompileCompleted())
                ? escapeJson(res.getDecompiledFunction().getC())
                : "";

            // 2. Assembly AArch64 e Coleta de Referências de Memória
            List<String> asmList = new ArrayList<>();
            Map<String, String> resolvedData = new HashMap<>();

            List<Instruction> insts = new ArrayList<>();
            InstructionIterator instructions = currentProgram.getListing().getInstructions(func.getBody(), true);
            while (instructions.hasNext()) {
                Instruction inst = instructions.next();
                insts.add(inst);
                asmList.add(escapeJson(inst.getAddress() + ": " + inst.toString()));

                for (Reference ref : inst.getReferencesFrom()) {
                    // Apenas leituras de dados: ignora fluxo (bl/b.ne/PLT), externos e não-.rodata
                    if (!ref.isMemoryReference() || ref.getReferenceType().isFlow()) {
                        continue;
                    }
                    Address to = ref.getToAddress();
                    MemoryBlock block = mem.getBlock(to);
                    if (block == null || !".rodata".equals(block.getName())) {
                        continue;
                    }
                    // Ignora strings (mensagens de log/printf)
                    Data d = currentProgram.getListing().getDataContaining(to);
                    if (d != null && d.hasStringValue()) {
                        continue;
                    }
                    String addrKey = "DAT_" + Long.toHexString(to.getOffset());
                    if (!resolvedData.containsKey(addrKey)) {
                        // tamanho do dado definido; senão 16 bytes (cobre registradores q de 128 bits)
                        int len = (d != null && d.isDefined() && d.getLength() > 1) ? d.getLength() : 16;
                        byte[] bytes = new byte[len];
                        int read = mem.getBytes(to, bytes);
                        if (read > 0) {
                            StringBuilder hex = new StringBuilder();
                            for (int i = 0; i < read; i++) {
                                hex.append(String.format("%02x ", bytes[i]));
                            }
                            resolvedData.put(addrKey, hex.toString().trim());
                        }
                    }
                }
            }

            // 3. Isolamento de loops: salto condicional para trás dentro da função
            List<String> loopsJson = new ArrayList<>();
            for (Instruction br : insts) {
                FlowType ft = br.getFlowType();
                if (!ft.isJump() || !ft.isConditional()) {
                    continue;
                }
                for (Address target : br.getFlows()) {
                    if (target.compareTo(br.getAddress()) >= 0 || !func.getBody().contains(target)) {
                        continue;
                    }
                    List<Instruction> body = new ArrayList<>();
                    Set<Register> liveIn = new HashSet<>();
                    for (Instruction li : insts) {
                        if (li.getAddress().compareTo(target) >= 0 && li.getAddress().compareTo(br.getAddress()) <= 0) {
                            body.add(li);
                            for (Object o : li.getInputObjects()) {
                                if (o instanceof Register) liveIn.add(((Register) o).getBaseRegister());
                            }
                        }
                    }
                    // Pré-cabeçalho: escritas (fora de chamadas) em registradores lidos no loop
                    List<Instruction> preheader = new ArrayList<>();
                    for (Instruction pi : insts) {
                        if (pi.getAddress().compareTo(target) >= 0) break;
                        if (pi.getFlowType().isCall()) continue;
                        for (Object o : pi.getResultObjects()) {
                            if (o instanceof Register && liveIn.contains(((Register) o).getBaseRegister())) {
                                preheader.add(pi);
                                break;
                            }
                        }
                    }
                    StringBuilder lj = new StringBuilder();
                    lj.append("        {\n");
                    lj.append("          \"start\": \"").append(target).append("\",\n");
                    lj.append("          \"end\": \"").append(br.getAddress()).append("\",\n");
                    lj.append("          \"preheader\": ").append(asmArray(preheader)).append(",\n");
                    lj.append("          \"body\": ").append(asmArray(body)).append("\n");
                    lj.append("        }");
                    loopsJson.add(lj.toString());
                }
            }

            json.append("    {\n");
            json.append("      \"name\": \"").append(func.getName()).append("\",\n");
            json.append("      \"entry_point\": \"").append(func.getEntryPoint().toString()).append("\",\n");
            json.append("      \"decompiled_c\": \"").append(cCode).append("\",\n");

            // Assembly
            json.append("      \"assembly\": [\n");
            for (int i = 0; i < asmList.size(); i++) {
                json.append("        \"").append(asmList.get(i)).append("\"").append(i < asmList.size() - 1 ? ",\n" : "\n");
            }
            json.append("      ],\n");

            // Dados estáticos resolvidos
            json.append("      \"resolved_data\": {\n");
            int dataIdx = 0;
            for (Map.Entry<String, String> entry : resolvedData.entrySet()) {
                json.append("        \"").append(entry.getKey()).append("\": \"").append(entry.getValue()).append("\"");
                if (++dataIdx < resolvedData.size()) json.append(",");
                json.append("\n");
            }
            json.append("      },\n");

            // Loops isolados
            json.append("      \"loops\": [\n").append(String.join(",\n", loopsJson)).append("\n      ]\n");
            json.append("    }");
        }

        json.append("\n  ]\n}");

        // nome de saída opcional: -postScript ExtractMain.java <arquivo.json>
        String[] args = getScriptArgs();
        String outName = args.length > 0 ? args[0] : "ghidra_extraction_hybrid.json";
        File out = new File(System.getProperty("user.dir"), outName);
        try (FileWriter writer = new FileWriter(out)) {
            writer.write(json.toString());
        }
        println("[+] Extracao hibrida (C + ASM + DAT) concluida -> " + out.getAbsolutePath());
    }

    private String asmArray(List<Instruction> list) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < list.size(); i++) {
            Instruction in = list.get(i);
            sb.append(i > 0 ? ", " : "").append("\"").append(escapeJson(in.getAddress() + ": " + in.toString())).append("\"");
        }
        return sb.append("]").toString();
    }

    private String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\")
                .replace("\"", "\\\"")
                .replace("\b", "\\b")
                .replace("\f", "\\f")
                .replace("\n", "\\n")
                .replace("\r", "\\r")
                .replace("\t", "\\t");
    }
}
