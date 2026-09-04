# Run after synth_1 completes and before impl_1 is launched.
open_project fpga.xpr
open_run synth_1

set debug_nets [lsort [get_nets -hier -filter {MARK_DEBUG == 1 && (NAME =~ *axis_udp_batch_aggregator_inst* || NAME =~ *agg_tx_*_count_*)}]]
if {![llength $debug_nets]} {
    error "No batch-aggregator MARK_DEBUG nets found"
}

set old_core [get_debug_cores -quiet u_batch_ila]
if {[llength $old_core]} {
    delete_debug_core $old_core
}
create_debug_core u_batch_ila ila
set_property C_DATA_DEPTH 1024 [get_debug_cores u_batch_ila]
set_property C_ADV_TRIGGER true [get_debug_cores u_batch_ila]
set_property C_INPUT_PIPE_STAGES 1 [get_debug_cores u_batch_ila]

# Vivado returns one net object per bit for MARK_DEBUG buses.  Group those
# objects by the common bus name so the ILA contains one 64-bit probe per
# counter instead of 1408 scalar probes.  Newer Vivado releases no longer
# expose C_NUM_OF_PROBES as a writable debug-core property, so add ports with
# create_debug_port.
array set debug_groups {}
foreach debug_net $debug_nets {
    set net_name [get_property NAME $debug_net]
    regsub {\[[0-9]+\]$} $net_name {} group_name
    lappend debug_groups($group_name) $debug_net
}

set probe_index 0
foreach group_name [lsort [array names debug_groups]] {
    if {$probe_index > 0} {
        create_debug_port u_batch_ila probe
    }
    set debug_port [get_debug_ports u_batch_ila/probe$probe_index]
    set group_nets [lsort -dictionary $debug_groups($group_name)]
    set_property port_width [llength $group_nets] $debug_port
    connect_debug_port $debug_port $group_nets
    puts "ILA_PROBE_${probe_index}=$group_name WIDTH=[llength $group_nets]"
    incr probe_index
}

# Synthesis flattens the aggregator's module-level clk pin.  Recover the exact
# clock net from an aggregation BRAM clock pin, whose primitive name is also
# checked by report_batch_post_synth.tcl.
set bram_clk_pin [get_pins -hier -filter {NAME =~ *axis_udp_batch_aggregator_inst/worker_mem_0_reg_bram_0/CLKARDCLK}]
set debug_clk [get_nets -of_objects $bram_clk_pin]
if {[llength $debug_clk] != 1} {
    error "Unable to resolve a unique ILA clock from the aggregation BRAM"
}
connect_debug_port u_batch_ila/clk $debug_clk
puts "ILA_CLOCK=$debug_clk PROBES=$probe_index"
save_constraints
close_project
