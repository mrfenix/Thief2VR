// Lists instructions whose text matches a regex, with the containing function.
// Usage: -postScript FindInstr.java <out_file> <regex> [start_hex end_hex]
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class FindInstr extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Pattern p = Pattern.compile(args[1]);
        Address start = args.length > 3 ? toAddr(args[2]) : null;
        Address end = args.length > 3 ? toAddr(args[3]) : null;
        int count = 0;
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            out.println("=== /" + args[1] + "/");
            InstructionIterator it = start != null ? currentProgram.getListing().getInstructions(start, true)
                                                   : currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && count < 400) {
                Instruction ins = it.next();
                if (end != null && ins.getAddress().compareTo(end) >= 0)
                    break;
                String text = ins.toString();
                if (p.matcher(text).find()) {
                    Function f = getFunctionContaining(ins.getAddress());
                    out.println(ins.getAddress() + "  " + text + "  in " + (f != null ? f.getName() : "?"));
                    ++count;
                }
            }
        }
    }
}
