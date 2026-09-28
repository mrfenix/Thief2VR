// Disassembles code Ghidra's auto-analysis skipped, starting at each address,
// and appends the listing (count instructions) to a file.
// Usage: -postScript DisasmForce.java <out_file> <addr>:<count> ...
// @category Thief2VR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DisasmForce extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                String[] parts = args[i].split(":");
                Address a = toAddr(parts[0]);
                int count = parts.length > 1 ? Integer.parseInt(parts[1]) : 40;
                disassemble(a);
                out.println("=== " + parts[0]);
                Instruction ins = getInstructionAt(a);
                for (int k = 0; k < count && ins != null; k++, ins = ins.getNext()) {
                    StringBuilder bytes = new StringBuilder();
                    for (byte b : ins.getBytes())
                        bytes.append(String.format("%02x ", b & 0xff));
                    out.println(String.format(" %s  %-24s %s", ins.getAddress(), bytes.toString().trim(), ins));
                }
            }
        }
    }
}
