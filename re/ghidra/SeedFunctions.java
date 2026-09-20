// Ghidra pre-script: creates functions at the entry points recorded by `gt2run calltrace`
// (functions.txt: "<hex address> <calls> <self instructions>" per line), so auto-analysis starts from
// code that is known to execute instead of guessing inside a raw RAM dump.
// Usage: -preScript SeedFunctions.java <functions.txt>
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

import java.io.BufferedReader;
import java.io.FileReader;

public class SeedFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int created = 0;
        try (BufferedReader reader = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.trim().split("\\s+");
                if (parts.length < 1 || parts[0].isEmpty()) continue;
                Address address = toAddr(Long.parseLong(parts[0], 16));
                new DisassembleCommand(address, null, true).applyTo(currentProgram, monitor);
                if (getFunctionAt(address) == null && createFunction(address, "fn_" + parts[0].toLowerCase()) != null) created++;
            }
        }
        println("SeedFunctions: created " + created + " functions");
    }
}
