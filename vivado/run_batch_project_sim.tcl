create_project -force batch_sim_project /tmp/vivado_batch_sim -part xczu9eg-ffvb1156-2-e
add_files -fileset sources_1 [file normalize ../rtl/axis_udp_batch_aggregator.v]
set_property file_type SystemVerilog [get_files axis_udp_batch_aggregator.v]
add_files -fileset sim_1 [file normalize test_axis_udp_batch_aggregator.sv]
set_property file_type SystemVerilog [get_files test_axis_udp_batch_aggregator.sv]
set_property top test_axis_udp_batch_aggregator [get_filesets sim_1]
launch_simulation -simset sim_1 -mode behavioral
run all
close_sim
close_project
