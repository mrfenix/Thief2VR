// For each argument substring, lists the defined strings that contain it and
// the functions that reference each one.
// Usage: -postScript StringXrefs.java <out_file> <substring> [<substring> ...]
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;

public class StringXrefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                String needle = args[i];
                out.println("=== \"" + needle + "\"");
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Data d = it.next();
                    StringDataInstance s = StringDataInstance.getStringDataInstance(d);
                    if (s == StringDataInstance.NULL_INSTANCE)
                        continue;
                    String value = s.getStringValue();
                    if (value == null || !value.contains(needle))
                        continue;
                    out.println(d.getAddress() + "  \"" + value.replace("\n", "\\n") + "\"");
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        out.println("    ref " + r.getFromAddress() + " in "
                                + (f == null ? "<no function>" : f.getName() + " @ " + f.getEntryPoint()));
                    }
                }
            }
        }
    }
}
