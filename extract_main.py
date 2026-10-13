# extract_main.py - Ghidra Headless Script
import json
import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

def run():
    program = currentProgram
    listing = program.getListing()
    fm = program.getFunctionManager()
    
    # Inicializa o descompilador do Ghidra
    decompiler = DecompInterface()
    decompiler.openProgram(program)
    monitor = ConsoleTaskMonitor()
    
    data = {
        "binary_name": program.getName(),
        "architecture": program.getLanguage().getProcessor().toString(),
        "functions": []
    }
    
    # Itera sobre todas as funções identificadas
    for func in fm.getFunctions(True):
        func_name = func.getName()
        
        # Filtra para focar nas funções relevantes (main ou funções de usuário)
        if "main" in func_name.lower():
            results = decompiler.decompileFunction(func, 60, monitor)
            decompiled_code = ""
            if results and results.decompileCompleted():
                decompiled_code = results.getDecompiledFunction().getC()
                
            entry_point = func.getEntryPoint().toString()
            body = func.getBody()
            instruction_count = listing.getInstructions(body, True).getNumInstructions()
            
            data["functions"].append({
                "name": func_name,
                "entry_point": entry_point,
                "instruction_count": instruction_count,
                "decompiled_c": decompiled_code
            })
            
    # Salva o resultado no mesmo diretório
    output_path = os.path.join(os.getcwd(), "ghidra_extraction.json")
    with open(output_path, "w") as fp:
        json.dump(data, fp, indent=2)
        
    print("[+] Extracao concluida com sucesso -> ghidra_extraction.json")

run()