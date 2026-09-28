// Decompiles the functions containing each argument address (hex) and appends
// the C output, plus the function's callers, to a file.
// Usage: -postScript Decompile.java <out_file> <addr> [<addr> ...]
// @category Thief2VR

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionContaining(a);
                if (f == null) {
                    out.println("=== " + args[i] + ": no function");
                    continue;
                }
                out.println("=== " + f.getName() + " @ " + f.getEntryPoint() + " (requested " + args[i] + ")");
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function caller = getFunctionContaining(r.getFromAddress());
                    out.println("// caller " + r.getFromAddress() + " in "
                            + (caller == null ? "<none>" : caller.getName()));
                }
                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC()
                                                      : "// decompile failed: " + res.getErrorMessage());
            }
        }
        decomp.dispose();
    }
}
