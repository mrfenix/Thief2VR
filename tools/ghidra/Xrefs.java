// Lists every reference to each argument address (hex), with the referencing
// function and whether it reads or writes.
// Usage: -postScript Xrefs.java <out_file> <addr> [<addr> ...]
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;

public class Xrefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                out.println("=== " + args[i]);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in "
                            + (f == null ? "<none>" : f.getName() + " @ " + f.getEntryPoint()));
                }
            }
        }
    }
}
