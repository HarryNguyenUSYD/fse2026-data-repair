package fsebenchmark.ddmax;

import inputrepair.program.DeltaDebugging;
import inputrepair.program.functional.TestOracle;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicInteger;

public final class DDMaxAdapter {
    private DDMaxAdapter() {
    }

    public static void main(String[] arguments) {
        if (arguments.length != 5) {
            System.err.println(
                    "Usage: DDMaxAdapter <validator> <input> <output> <timeout-ms> <suffix>");
            System.exit(2);
        }

        final Path validator = Path.of(arguments[0]).toAbsolutePath();
        final Path input = Path.of(arguments[1]);
        final Path output = Path.of(arguments[2]);
        final long timeout;
        try {
            timeout = Long.parseLong(arguments[3]);
        } catch (NumberFormatException exception) {
            System.err.println("Invalid timeout: " + arguments[3]);
            System.exit(2);
            return;
        }
        final String suffix = arguments[4];
        final AtomicInteger oracleRuns = new AtomicInteger();

        try {
            final String corrupt = Files.readString(input, StandardCharsets.UTF_8);
            final TestOracle oracle = candidate -> {
                oracleRuns.incrementAndGet();
                try {
                    Process process = new ProcessBuilder(
                            validator.toString(), candidate.toAbsolutePath().toString())
                            .redirectOutput(ProcessBuilder.Redirect.DISCARD)
                            .redirectError(ProcessBuilder.Redirect.DISCARD)
                            .start();
                    int status = process.waitFor();
                    if (status == 0) return true;
                    if (status == 1) return false;
                    throw new IllegalStateException(
                            "Validator exited with unexpected status " + status);
                } catch (IOException exception) {
                    throw new IllegalStateException("Could not execute validator", exception);
                } catch (InterruptedException exception) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("Validator execution interrupted", exception);
                }
            };

            final String repaired = DeltaDebugging.ddmax(
                    corrupt, oracle, timeout, suffix);
            Files.writeString(output, repaired, StandardCharsets.UTF_8);
            System.out.println("DDMAX_ORACLE_RUNS=" + oracleRuns.get());
        } catch (Exception exception) {
            exception.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
