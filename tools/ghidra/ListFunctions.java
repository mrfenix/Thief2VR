// Lists functions whose entry lies in [start, end) with size and caller count.
// Usage: -postScript ListFunctions.java <out_file> <start_hex> <end_hex>
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ListFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Address start = toAddr(args[1]);
        Address end = toAddr(args[2]);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(start, true);
            while (it.hasNext()) {
                Function f = it.next();
                if (f.getEntryPoint().compareTo(end) >= 0)
                    break;
                int callers = getReferencesTo(f.getEntryPoint()).length;
                out.println(String.format("%s  size %5d  refs %4d  %s", f.getEntryPoint(), f.getBody().getNumAddresses(),
                        callers, f.getSignature().getPrototypeString()));
            }
        }
    }
}
