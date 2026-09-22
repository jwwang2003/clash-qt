# Coordinator shared-contract integration note

The complete shared rows are part of backend-real-contract-tests, retaining its
specialist cases. CMake now registers the two new .cpp files, host/marshal/fake
links and both module artifact paths; no new private-implementation linking
exception is created. No extra CTest entry duplicates the same assertions.

Module availability in this integration lane is REQUIRED. A bad module load or
missing artifact must fail, rather than QSKIP a required row while CTest prints
Passed. Do not weaken assertions to fit the adapter; report adapter failures to
the ABI repair lease. Final qualification explicitly checks there are no skipped
required module rows. Missing Windows/Linux runners remain a separate G5 gap.
