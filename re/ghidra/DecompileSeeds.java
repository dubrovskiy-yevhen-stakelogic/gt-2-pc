// Ghidra post-script: decompiles every function listed in functions.txt (see SeedFunctions.java) into
// <outDir>/<address>.c. The output is a READING AID for the port - it is derived from the game's code and
// therefore stays under work\ (gitignored), never in the repository.
// Usage: -postScript DecompileSeeds.java <functions.txt> <outDir>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;

public class DecompileSeeds extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[1]);
        outDir.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        int done = 0, failed = 0;
        try (BufferedReader reader = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.trim().split("\\s+");
                if (parts.length < 1 || parts[0].isEmpty()) continue;
                Function function = getFunctionAt(toAddr(Long.parseLong(parts[0], 16)));
                if (function == null) { failed++; continue; }
                DecompileResults results = decompiler.decompileFunction(function, 120, monitor);
                if (!results.decompileCompleted()) { failed++; continue; }
                try (PrintWriter writer = new PrintWriter(new File(outDir, parts[0].toUpperCase() + ".c"))) {
                    writer.println("// " + function.getName() + "  calls=" + (parts.length > 1 ? parts[1] : "?") +
                                   " selfInstructions=" + (parts.length > 2 ? parts[2] : "?"));
                    writer.print(results.getDecompiledFunction().getC());
                }
                done++;
            }
        }
        println("DecompileSeeds: " + done + " decompiled, " + failed + " failed");
    }
}
