package inputrepair.program;

import inputrepair.program.functional.FileGenerator;
import inputrepair.program.functional.TestOracle;
import inputrepair.program.visitor.SimpleTreeFlattener;
import org.antlr.v4.runtime.tree.ParseTree;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;
import java.util.logging.Level;

/**
 * Delta Debugging class for maximizing Inputs.
 * Used for the implementation of the syntactic and lexical DDMax algorithm.
 *
 * @param <T> Type of the data that should be maximized, e.g. String for lexical
 * @author Lukas Kirschner
 */
public class DeltaDebugging<T> extends Loggable {
    private static int oracleRunsForString = 0;
    private static int oracleRunsForTree = 0;
    private final long timeout;
    private final long starttime;
    private final String fileSuffix;
    private Path tempfolder;
    private TestOracle oracle;
    private T input;
    private Function<T, Integer> lengthGetter;
    private FileGenerator<T> fileGenerator;
    private int numberOfOracleRuns = 0;

    /**
     * @param input         The initial input
     * @param oracle        Lambda that validates an input
     * @param lengthGetter  Lambda that gets the length of an input
     * @param fileGenerator Lambda that transform an intermediate representation of the input to a version that can be printed into a file for an oracle
     * @param timeout       Minimum amount of time in ms that a DD run is allowed to take
     * @param fileSuffix    Suffix (file extension) of a file that can be tested by the oracle, which is the file format of the input under test
     */
    private DeltaDebugging(T input,
                           TestOracle oracle,
                           Function<T, Integer> lengthGetter,
                           FileGenerator<T> fileGenerator,
                           long timeout,
                           String fileSuffix) {
        super("DDMax");
        this.fileSuffix = fileSuffix;
        this.timeout = timeout;
        this.oracle = oracle;
        this.input = input;
        this.lengthGetter = lengthGetter;
        this.fileGenerator = fileGenerator;
        this.starttime = System.currentTimeMillis();
        try {
            this.tempfolder = Files.createTempDirectory("deltadebugging");
        } catch (IOException e) {
            this.tempfolder = null;
            log(Level.SEVERE, "Could not create temporary directory!", e);
        }
    }

    /**
     * Implementation of the ddmax algorithm for Strings
     *
     * @param input   Input string
     * @param oracle  Oracle for testing
     * @param timeout Minimum amount of time in ms that a DD run is allowed to take
     * @param suffix  Suffix (file extension) of a file that can be tested by the oracle, which is the file format of the input under test
     * @return maximized string that passes the oracle
     */
    public static String ddmax(String input, TestOracle oracle, long timeout, String suffix) {
        DeltaDebugging<String> deltaDebugging = new DeltaDebugging<>(input, oracle, String::length, (d, s) -> exclude(s, d), timeout, suffix);
        deltaDebugging.log(Level.INFO, "Running DDMax...");
        var set = deltaDebugging.ddmax_recursive(new DeltaSet(0, input.length()), 2);
        var res = exclude(input, set);
        oracleRunsForString += deltaDebugging.getNumberOfOracleRuns();
        return res;
    }

    /**
     * Implementation of the ddmin algorithm for Strings
     *
     * @param input   Input string
     * @param oracle  Oracle for testing
     * @param timeout Minimum amount of time in ms that a DD run is allowed to take
     * @param suffix  Suffix (file extension) of a file that can be tested by the oracle, which is the file format of the input under test
     * @return Minimized string that fails the oracle and passes ANTLR
     */
    public static String ddmin(String input, TestOracle oracle, long timeout, String suffix) {
        DeltaDebugging<String> deltaDebugging = new DeltaDebugging<>(input, oracle, String::length, (d, s) -> intersect(s, d), timeout, suffix);
        deltaDebugging.log(Level.INFO, "Running DDMin...");
        var set = deltaDebugging.ddmin_recursive(new DeltaSet(0, input.length()), 2);
        var res = intersect(input, set);
//        oracleRunsForString += deltaDebugging.getNumberOfOracleRuns();
        return res;
    }

    /**
     * Implementation of the grammar-aware ddmax algorithm for ANTLR ParseTrees
     *
     * @param input   Input string
     * @param oracle  Oracle for testing
     * @param timeout Timeout in ms
     * @param suffix  Suffix of the file under test, used to generate temporary files for the subject program. We need this for subject programs that check the file suffix of the format to determine the correct input format.
     * @return maximized string that passes the oracle
     */
    public static String ddmax(ParseTree input, TestOracle oracle, long timeout, String suffix) {
        final List<String> flattenedTree = input.accept(new SimpleTreeFlattener());
        FileGenerator<List<String>> ex = (exclusionSet, stringList) -> {
            final List<String> excluded = exclude(stringList, exclusionSet);
            return String.join("", excluded);//Correct number of whitespaces is determined by SimpleTreeFlattener
        };
        Function<List<String>, Integer> len = List::size;
        DeltaDebugging<List<String>> dd = new DeltaDebugging<>(
                flattenedTree, oracle, len,
                ex, timeout, suffix);
        dd.log(Level.INFO, "Running DDMaxG...");
        var set = dd.ddmax_recursive(new DeltaSet(0, len.apply(flattenedTree)), 2);
        var res = ex.feed(set, flattenedTree);
        oracleRunsForTree += dd.getNumberOfOracleRuns();
        return res;
    }

    /**
     * Excludes the given DeltaSet from a string.
     *
     * @param input    String to use for exclusion
     * @param deltaSet DeltaSet to exclude
     * @return non-excluded part of the string
     */
    public static String exclude(String input, DeltaSet deltaSet) {
        StringBuilder ret = new StringBuilder(input.length() - deltaSet.length());
        for (int i = 0; i < input.length(); i++) {
            if (!deltaSet.inside(i)) {
                ret.append(input.charAt(i));
            }
        }
        return ret.toString();
    }

    /**
     * Intersects the given DeltaSet with a string.
     *
     * @param input    String to use for  intersection
     * @param deltaSet DeltaSet to intersect
     * @return Intersected part of the string
     */
    public static String intersect(String input, DeltaSet deltaSet) {
        StringBuilder ret = new StringBuilder(input.length() - deltaSet.length());
        for (int i = 0; i < input.length(); i++) {
            if (deltaSet.inside(i)) {
                ret.append(input.charAt(i));
            }
        }
        return ret.toString();
    }

    /**
     * Excludes given DeltaSet from a list
     *
     * @param input    List to exclude from
     * @param deltaSet DeltaSet to exclude
     * @param <X>      Type of the list elements
     * @return List without the excluded parts
     */
    public static <X> List<X> exclude(List<X> input, DeltaSet deltaSet) {
        ArrayList<X> ret = new ArrayList<>(input.size() - deltaSet.length());
        for (int i = 0; i < input.size(); i++) {
            if (!deltaSet.inside(i)) {
                ret.add(input.get(i));
            }
        }
        return ret;
    }

    /**
     * Generates a new DeltaInterval for the given granularity.
     *
     * @param deltaSet    DeltaSet, which is the environment the new DeltaInterval is based upon
     * @param granularity Granularity
     * @param i           Index of the Delta, between 0 and granularity-1
     * @return DeltaInterval
     */
    public static DeltaInterval getGranularityInterval(DeltaSet deltaSet, int granularity, int i) {
        final int granuLength = deltaSet.length() / granularity;
        final int lb = deltaSet.getNthIndex(i * granuLength);
        final int ub;
        if (i != granularity - 1) {
            ub = deltaSet.getNthIndex((i + 1) * granuLength - 1) + 1;
        } else {
            ub = deltaSet.getNthIndex(deltaSet.length() - 1) + 1;
        }
        return new DeltaInterval(lb, ub);
    }

    /**
     * Gets the number of oracle runs for a test run
     *
     * @return Number of Oracle Runs
     */
    public int getNumberOfOracleRuns() {
        return numberOfOracleRuns;
    }

    /**
     * Runs the oracle
     *
     * @param excludingSet DeltaSet that is excluded from the test input
     * @return true, if test run succeeded
     */
    private boolean runOracle(DeltaSet excludingSet) {
        String bytearray = fileGenerator.feed(excludingSet, input);
        boolean result = false;
        Path ddfile = Paths.get(tempfolder.normalize().toString(), "deltadbg_" + System.currentTimeMillis() + "." + this.fileSuffix);
        numberOfOracleRuns++;
        try {
            Files.write(ddfile, bytearray.getBytes(StandardCharsets.UTF_8), StandardOpenOption.WRITE, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
            log(Level.FINE, String.format("Oracle Run %04d, testing file %s",
                    numberOfOracleRuns,
                    ddfile.toString()));
            result = oracle.run(ddfile);
        } catch (IOException e) {
            log(Level.SEVERE, "Could not write temporary file", e);
        }
        try {
            Files.delete(ddfile);
        } catch (IOException e) {
            log(Level.SEVERE, "Could not delete temporary file", e);
        }

        return result;//Return true if oracle passes?
    }

    /**
     * Checks if a string can be parsed by ANTLR.
     * @param excludingSet Exclusion set to exclude from the string
     * @return true, if ANTLR parses the string
     */
    private boolean antlrParses(DeltaSet excludingSet) {
        String bytearray = fileGenerator.feed(excludingSet, input);
        boolean result = false;
        Path ddfile = Paths.get(tempfolder.normalize().toString(), "deltadbg_" + System.currentTimeMillis() + "." + this.fileSuffix);
        try {
            Files.write(ddfile, bytearray.getBytes(StandardCharsets.UTF_8), StandardOpenOption.WRITE, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
            log(Level.FINE, String.format("Oracle Run %04d, testing file %s",
                    numberOfOracleRuns,
                    ddfile.toString()));
            switch (this.fileSuffix) {
                case ".obj":
                    return Parsing.parseWavefrontOBJ(ddfile.normalize().toString(), true) != null;
                case ".json":
                    return Parsing.parseJSON(ddfile.normalize().toString(), true) != null;
                case ".dot":
                    return Parsing.parseDOT(ddfile.normalize().toString(), true) != null;
                default:
                    throw new RuntimeException("Unknown Format " + this.fileSuffix);
            }
        } catch (IOException e) {
            log(Level.SEVERE, "Could not write temporary file", e);
        }
        try {
            Files.delete(ddfile);
        } catch (IOException e) {
            log(Level.SEVERE, "Could not delete temporary file", e);
        }

        return result;//Return true if oracle passes?
    }

    /**
     * The recursive implementation of DDmin.
     * Minimizes a failing input, while the minimized input still fails and is parsed by ANTLR nonetheless
     *
     * @param deltaSet    Start Set
     * @param granularity Start Granularity
     * @return the minimized DeltaSet
     */
    private DeltaSet ddmin_recursive(DeltaSet deltaSet, int granularity) {
        final long elapsed = (System.currentTimeMillis() - starttime);
//        System.out.printf("Elapsed: %d Timeout: %d%n",elapsed,timeout);
        if (this.timeout != -1 && elapsed > timeout) {
            log(Level.WARNING, String.format("Timeout with %dms", elapsed));
            return deltaSet;
        }

        final int inpLength = deltaSet.length();
        if (inpLength == 1) {//1-minimal
            log(Level.INFO, "DeltaSet is 1-minimal");
            return deltaSet;
        }
        if (inpLength == 0) {
            log(Level.WARNING, "Skipping DDMax because DeltaSet is empty");
            return deltaSet; // Handle cases where the interval is empty
        }

        for (int i = 0; i < granularity; i++) {//Check intervals
            final DeltaInterval jointInterval = getGranularityInterval(deltaSet, granularity, i);
            final DeltaSet newSet = new DeltaSet(jointInterval);
            if (!this.runOracle(newSet) && this.antlrParses(newSet)) {
                log(Level.INFO, "ddmin accepted a jointDeltaSet. Continuing using " + newSet);
                return ddmin_recursive(newSet, 2);
            }
        }
        for (int i = 0; i < granularity; i++) {//Check intervals
            final DeltaInterval exclInterval = getGranularityInterval(deltaSet, granularity, i);
            final DeltaSet newSet = new DeltaSet(deltaSet);
            newSet.excludeInterval(exclInterval);
            if (!this.runOracle(newSet) && this.antlrParses(newSet)) {
                log(Level.INFO, "ddmin accepted an exclSet. Continuing using " + newSet);
                return ddmin_recursive(newSet, 2);
            }
        }
        if (granularity < inpLength) {
            log(Level.INFO, "Increased Granularity to " + Math.min(inpLength, 2 * granularity));
            return ddmin_recursive(deltaSet, Math.min(inpLength, 2 * granularity));
        }
        log(Level.INFO, "DeltaSet is minimal");
        return deltaSet;
    }

    /**
     * The recursive implementation of Delta Debugging for Maximizing Inputs
     *
     * @param deltaSet    Start DeltaSet which is used as ExclusionSet
     * @param granularity Granularity
     * @return resulting Exclusion Set
     */
    private DeltaSet ddmax_recursive(DeltaSet deltaSet, int granularity) {
        final long elapsed = (System.currentTimeMillis() - starttime);
        if (this.timeout != -1 && elapsed > timeout) {
            log(Level.WARNING, String.format("Timeout with %dms", elapsed));
            return deltaSet;
        }
        //Check Delta Subsets
        final int inpLength = deltaSet.length();
        if (inpLength == 1) {//1-minimal
            log(Level.INFO, "DeltaSet is 1-minimal");
            return deltaSet;
        }
        if (inpLength == 0) {
            log(Level.WARNING, "Skipping DDMax because DeltaSet is empty");
            return deltaSet; // Handle cases where the interval is empty
        }
        for (int i = 0; i < granularity; i++) {//Check intervals
            final DeltaInterval jointInterval = getGranularityInterval(deltaSet, granularity, i);
            final DeltaSet newSet = new DeltaSet(jointInterval);
            if (this.runOracle(newSet)) {
                log(Level.INFO, "ddmax accepted a jointDeltaSet. Continuing using " + newSet);
                return ddmax_recursive(newSet, 2);
            }
        }
        for (int i = 0; i < granularity; i++) {//Check intervals
            final DeltaInterval exclInterval = getGranularityInterval(deltaSet, granularity, i);
            final DeltaSet newSet = new DeltaSet(deltaSet);
            newSet.excludeInterval(exclInterval);
            if (this.runOracle(newSet)) {
                log(Level.INFO, "ddmax accepted an exclSet. Continuing using " + newSet);
                return ddmax_recursive(newSet, 2);
            }
        }
        if (granularity < inpLength) {
            log(Level.INFO, "Increased Granularity to " + Math.min(inpLength, 2 * granularity));
            return ddmax_recursive(deltaSet, Math.min(inpLength, 2 * granularity));
        }
        log(Level.INFO, "DeltaSet is minimal");
        return deltaSet;
    }
}