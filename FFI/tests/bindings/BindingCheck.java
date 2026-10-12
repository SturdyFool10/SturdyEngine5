import dev.sturdy.Sturdy;
import java.lang.foreign.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

/** Checks the generated Java binding against the real header (layout table) and calls every wrapper through a stub library. */
public class BindingCheck {
    public static void main(String[] args) throws Throwable {
        int failures = 0;
        // ---- layouts ----
        int structs = 0, fields = 0;
        for (String line : Files.readAllLines(Path.of(args[0]))) {
            String[] p = line.trim().split("\\s+");
            String name = p[0];
            long size = Long.parseLong(p[1]), align = Long.parseLong(p[2]);
            MemoryLayout layout = (MemoryLayout) Sturdy.class.getField("LAYOUT_" + name).get(null);
            structs++;
            if (layout.byteSize() != size || layout.byteAlignment() != align) {
                System.out.println("MISMATCH size/align " + name + " " + layout.byteSize() + "/" + layout.byteAlignment() + " vs " + size + "/" + align);
                failures++;
            }
            long constant = (long) Sturdy.class.getField("SIZE_" + name).get(null);
            if (constant != size) { System.out.println("MISMATCH SIZE_ " + name); failures++; }
            for (int i = 3; i < p.length; i++) {
                String[] kv = p[i].split("=");
                long offset = (long) Sturdy.class.getField("OFFSET_" + name + "_" + kv[0]).get(null);
                fields++;
                if (offset != Long.parseLong(kv[1])) { System.out.println("MISMATCH offset " + name + "." + kv[0] + " " + offset + " vs " + kv[1]); failures++; }
                // The layout itself must agree too (Panama computes the offset from the layout).
                long fromLayout = ((StructLayout) layout).byteOffset(MemoryLayout.PathElement.groupElement(kv[0]));
                if (fromLayout != Long.parseLong(kv[1])) { System.out.println("MISMATCH layout offset " + name + "." + kv[0]); failures++; }
            }
        }
        System.out.println("java layouts: " + structs + " structs, " + fields + " fields checked against the real header; mismatches: " + failures);

        // ---- every function, through the stub ----
        Sturdy.load(Path.of(args[1]));   // the stub reports the ABI version the binding expects
        int calls = 0, bad = 0;
        try (Arena arena = Arena.ofConfined()) {
            for (Method m : Sturdy.class.getDeclaredMethods()) {
                if (!m.getName().startsWith("sturdy_") || !Modifier.isPublic(m.getModifiers())) continue;
                Class<?>[] types = m.getParameterTypes();
                Object[] values = new Object[types.length];
                for (int i = 0; i < types.length; i++) {
                    Class<?> t = types[i];
                    if (t == int.class) values[i] = 0;
                    else if (t == long.class) values[i] = 0L;
                    else if (t == short.class) values[i] = (short) 0;
                    else if (t == byte.class) values[i] = (byte) 0;
                    else if (t == boolean.class) values[i] = false;
                    else if (t == float.class) values[i] = 0f;
                    else if (t == double.class) values[i] = 0d;
                    else if (t == SegmentAllocator.class) values[i] = arena;
                    else values[i] = arena.allocate(4096);   // a pointer to zeroed memory, or room for a struct passed by value
                }
                try {
                    m.invoke(null, values);
                    calls++;
                } catch (InvocationTargetException e) {
                    bad++;
                    System.out.println("CALL FAILED " + m.getName() + ": " + e.getCause());
                }
            }
        }
        System.out.println("java wrappers: " + calls + " called through the stub, " + bad + " failed");
        if (failures + bad != 0) System.exit(1);
    }
}
