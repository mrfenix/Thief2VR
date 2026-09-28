// Appends a disassembly listing around each address to a file, with raw bytes
// (useful for writing byte signatures).
// Usage: -postScript Disasm.java <out_file> <addr>[:<before>:<after>] ...
// before/after are instruction counts (default 12/24).
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

import java.io.FileWriter;
import java.io.PrintWriter;

public class Disasm extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                String[] parts = args[i].split(":");
                Address a = toAddr(parts[0]);
                int before = parts.length > 1 ? Integer.parseInt(parts[1]) : 12;
                int after = parts.length > 2 ? Integer.parseInt(parts[2]) : 24;
                Function f = getFunctionContaining(a);
                out.println("=== " + parts[0] + (f != null ? " in " + f.getName() : ""));
                Instruction ins = getInstructionContaining(a);
                if (ins == null) {
                    out.println("  no instruction");
                    continue;
                }
                for (int k = 0; k < before && ins.getPrevious() != null; k++)
                    ins = ins.getPrevious();
                for (int k = 0; k < before + after && ins != null; k++, ins = ins.getNext()) {
                    StringBuilder bytes = new StringBuilder();
                    for (byte b : ins.getBytes())
                        bytes.append(String.format("%02x ", b & 0xff));
                    out.println(String.format("%s%s  %-24s %s", ins.getAddress().equals(a) ? ">" : " ",
                            ins.getAddress(), bytes.toString().trim(), ins.toString()));
                }
            }
        }
    }
}
