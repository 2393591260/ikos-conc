package com.dat3m.dartagnan.verification.solving;

import com.dat3m.dartagnan.encoding.EncodingContext;
import com.dat3m.dartagnan.encoding.ProverWithTracker;
import com.dat3m.dartagnan.program.event.Event;
import com.dat3m.dartagnan.program.event.Tag;
import com.dat3m.dartagnan.program.event.core.MemoryCoreEvent;
import com.dat3m.dartagnan.program.event.metadata.MemoryOrder;
import com.dat3m.dartagnan.program.event.metadata.SourceLocation;
import org.sosy_lab.java_smt.api.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.math.BigInteger;
import java.security.MessageDigest;
import java.util.*;

/** Research adapter for pinned Dat3M 7caef5c; NOT a competition witness validator. */
public final class StrictWitness {
    public static boolean enabled() { return System.getenv("IKOS_STRICT_CERTIFICATE") != null; }
    private static String b64(String s) {
        return Base64.getEncoder().encodeToString(s.getBytes(StandardCharsets.UTF_8));
    }
    private static String descriptor(Event e) {
        var source = e.getMetadata(SourceLocation.class);
        var creator = e.getThread().getEntry().getCreator();
        return e.getGlobalId() + "\t" + e.getThread().getId() + "\t"
            + (creator == null ? -1 : creator.getGlobalId()) + "\t"
            + b64(source == null ? "" : source.sourceCodeFilePath()) + "\t"
            + (source == null ? -1 : source.lineNumber()) + "\t" + b64(e.toString());
    }
    private static BooleanFormula number(EncodingContext c, Formula f, String value) {
        if (f == null) throw new IllegalArgumentException("value has no encoded source");
        var fm = c.getFormulaManager();
        var n = new BigInteger(value);
        if (f instanceof BitvectorFormula bv) {
            var m = fm.getBitvectorFormulaManager();
            int bits = m.getLength(bv);
            // Certificate integers represent raw unsigned bit patterns; never silently wrap.
            if (n.signum() < 0 || n.bitLength() > bits) throw new IllegalArgumentException("out-of-range bit pattern");
            return m.equal(bv, m.makeBitvector(bits, n));
        }
        if (f instanceof NumeralFormula.IntegerFormula i) {
            var m = fm.getIntegerFormulaManager();
            return m.equal(i, m.makeNumber(n));
        }
        throw new IllegalArgumentException("unsupported value representation: " + f.getClass());
    }
    private static Event event(Properties p, String key, Map<Integer, Event> events) {
        String id = p.getProperty(key);
        if (id == null) throw new IllegalArgumentException("missing " + key);
        Event e = events.get(Integer.parseInt(id));
        if (e == null) throw new IllegalArgumentException("unknown event: " + id);
        return e;
    }
    private static boolean canRace(MemoryCoreEvent e) {
        var mo = e.getMetadata(MemoryOrder.class);
        return !e.hasTag(Tag.INIT) && (mo == null || mo.value().equals(Tag.C11.NONATOMIC));
    }
    // Restrict the existing native race query to exactly this pair. This is CAT hb,
    // not POSIX hb or IKOS's scheduler clock. Do not translate total order to this edge.
    private static BooleanFormula race(EncodingContext c, MemoryCoreEvent w, MemoryCoreEvent m) {
        var bm = c.getBooleanFormulaManager();
        if (!w.hasTag(Tag.WRITE) || w.getThread() == m.getThread() || !canRace(w) || !canRace(m)
            || (w.hasTag(Tag.RMW) && m.hasTag(Tag.RMW))) return bm.makeFalse();
        var im = c.getFormulaManager().getIntegerFormulaManager();
        return bm.and(c.execution(w, m), c.sameAddress(w, m),
            c.edge(c.getTask().getMemoryModel().getRelation("hb"), m, w),
            im.equal(c.clockVariable("hb", w), im.add(c.clockVariable("hb", m), im.makeNumber(1))));
    }
    public static void constrain(EncodingContext c, ProverWithTracker prover) {
        try {
            var events = new TreeMap<Integer, Event>();
            var lines = new ArrayList<String>();
            for (var thread : c.getTask().getProgram().getThreads()) for (var e : thread.getEvents()) {
                events.put(e.getGlobalId(), e);
                lines.add(descriptor(e));
            }
            String snapshot = String.join("\n", lines) + "\n";
            String hash = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
                .digest(snapshot.getBytes(StandardCharsets.UTF_8)));
            String dump = System.getenv("IKOS_STRICT_EVENT_DUMP");
            if (dump != null) Files.writeString(Path.of(dump), hash + "\n" + snapshot);
            if (!enabled()) return;
            Properties p = new Properties();
            try (var reader = Files.newBufferedReader(Path.of(System.getenv("IKOS_STRICT_CERTIFICATE")))) { p.load(reader); }
            if (!"1".equals(p.getProperty("schema")) || !hash.equals(p.getProperty("ir_sha256")))
                throw new IllegalArgumentException("schema or processed IR mismatch");
            var used = new HashSet<String>(List.of("schema", "ir_sha256", "count", "target_a", "target_b", "edge_count"));
            var bm = c.getBooleanFormulaManager();
            int count = Integer.parseInt(p.getProperty("count"));
            if (count < 2 || count > 10000) throw new IllegalArgumentException("invalid event count");
            var boundIds = new HashSet<Integer>();
            for (int i = 0; i < count; ++i) {
                String k = "event." + i + ".";
                used.addAll(List.of(k + "id", k + "thread", k + "creator", k + "descriptor", k + "value", k + "address"));
                Event e = event(p, k + "id", events);
                if (!boundIds.add(e.getGlobalId())) throw new IllegalArgumentException("duplicate event");
                var creator = e.getThread().getEntry().getCreator();
                if (!Integer.toString(e.getThread().getId()).equals(p.getProperty(k + "thread"))
                    || !Integer.toString(creator == null ? -1 : creator.getGlobalId()).equals(p.getProperty(k + "creator"))
                    || !b64(descriptor(e)).equals(p.getProperty(k + "descriptor")))
                    throw new IllegalArgumentException("event/instance/creation mismatch");
                prover.addConstraint(c.execution(e));
                if (p.containsKey(k + "value") || p.containsKey(k + "address")) {
                    if (!(e instanceof MemoryCoreEvent m)) throw new IllegalArgumentException("not a memory event");
                    if (p.containsKey(k + "value")) prover.addConstraint(number(c, c.value(m).formula(), p.getProperty(k + "value")));
                    if (p.containsKey(k + "address")) prover.addConstraint(number(c, c.address(m).formula(), p.getProperty(k + "address")));
                }
            }
            Event a = event(p, "target_a", events), b = event(p, "target_b", events);
            if (!boundIds.contains(a.getGlobalId()) || !boundIds.contains(b.getGlobalId())
                || !(a instanceof MemoryCoreEvent ma) || !(b instanceof MemoryCoreEvent mb))
                throw new IllegalArgumentException("targets must be bound memory events");
            prover.addConstraint(bm.or(race(c, ma, mb), race(c, mb, ma)));
            int edges = Integer.parseInt(p.getProperty("edge_count", "0"));
            if (edges < 0 || edges > 10000) throw new IllegalArgumentException("invalid edge count");
            for (int i = 0; i < edges; ++i) {
                String k = "edge." + i + ".";
                used.addAll(List.of(k + "relation", k + "from", k + "to"));
                String relation = p.getProperty(k + "relation");
                if (!Set.of("rf", "co", "hb").contains(relation)) throw new IllegalArgumentException("unsupported relation");
                Event from = event(p, k + "from", events), to = event(p, k + "to", events);
                if (!boundIds.contains(from.getGlobalId()) || !boundIds.contains(to.getGlobalId()))
                    throw new IllegalArgumentException("edge references unbound event");
                prover.addConstraint(c.edge(c.getTask().getMemoryModel().getRelation(relation), from, to));
            }
            if (!used.containsAll(p.stringPropertyNames())) throw new IllegalArgumentException("unknown certificate field");
            System.out.println("REFERENCE_CERTIFICATE_BOUND events=" + count + " edges=" + edges);
        } catch (Exception e) {
            // Fail closed; caller must treat this as UNSUPPORTED, never ACCEPT.
            throw new IllegalArgumentException("REFERENCE_CERTIFICATE_UNSUPPORTED: " + e.getMessage(), e);
        }
    }
}
