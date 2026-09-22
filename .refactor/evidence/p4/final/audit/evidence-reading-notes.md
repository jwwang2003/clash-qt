# Evidence interpretation

The qualified source is be851f5. Audit logs include deliberately failing mutations,
failed setup attempts, and concurrent-preference guard detection. They are not a
collection of green-only runs. The prefprobe-1/2/3 logs retained stable preference
hashes but failed two fixture-start cases because those manual probes used an
incorrect relative fixture path; they are not evidence of a passing contract run.
The restored-ctest.log and restored2-ctest.log each pass all four registered suites.
The original-verdict.md preserves the audit's original statement; its preference
attribution is explicitly superseded by the follow-up correction. Runtime notes
section8 has been corrected by an independent Opus follow-up.

The correction report's references to an old qualification "After" value describe
its historical snapshot. The current P4_QUALIFICATION.md now separates the unchanged
qualification-run hashes from later geometry changes. First change overlaps confirmed
user GUI use; that is the supported explanation, not an instrumented write trace.
The later transition has no identified writer. A hash guard remains useful as a
contamination detector; it cannot attribute a writer or establish that opening a
QSettings domain causes a flush. No such mechanism is proven by these logs.
