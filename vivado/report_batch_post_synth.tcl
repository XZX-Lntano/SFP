open_project fpga.xpr
open_run synth_1
report_utilization -hierarchical -hierarchical_depth 5 -file batch_post_synth_utilization.rpt
report_timing_summary -delay_type max -max_paths 20 -file batch_post_synth_timing.rpt
set agg_cells [get_cells -hier -filter {NAME =~ *axis_udp_batch_aggregator_inst*}]
set agg_brams [filter $agg_cells {REF_NAME =~ RAMB*}]
puts "BATCH_AGGREGATOR_BRAM_PRIMITIVES=[llength $agg_brams]"
foreach cell $agg_brams { puts "BATCH_BRAM=$cell REF=[get_property REF_NAME $cell]" }
close_project
