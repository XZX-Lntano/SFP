# Aggregator self-recovery CDC - intentionally does nothing.
#
# The recovery pulse is brought into each port clock domain with the library
# sync_reset module in rtl/fpga_core.v; lib/axis/syn/vivado/sync_reset.tcl
# already sets ASYNC_REG and false-paths the synchroniser reset inputs, so no
# hand-written constraint is required. This file exists only because the
# project's constraints fileset references it.
