open_run impl_1
report_timing_summary -file $root/reports/timing_summary.rpt
report_utilization -file $root/reports/utilization.rpt
report_drc -file $root/reports/drc.rpt
report_methodology -file $root/reports/methodology.rpt
# Check the final design, including changes made by post-route optimization.
set skew_path $root/reports/bus_skew.rpt
report_bus_skew -file $skew_path
set f [open $skew_path r]
set skew [read $f]
close $f
if {[string first "Slack (VIOLATED)" $skew] >= 0 || [string first "Slack (MET)" $skew] < 0} {
    error "Bus skew did not pass"
}
set f [open $root/reports/timing_summary.rpt r]
set timing [read $f]
close $f
if {[string first "All user specified timing constraints are met." $timing] < 0} {
    error "Timing summary did not pass"
}
set setup [get_timing_paths -delay_type max -max_paths 1]
set hold [get_timing_paths -delay_type min -max_paths 1]
if {![llength $setup] || ![llength $hold]} {error "Missing timing paths"}
if {[get_property SLACK $setup] < 0 || [get_property SLACK $hold] < 0} {error "Timing did not close"}
file copy -force $root/build/accelerator/accelerator.runs/impl_1/accelerator_wrapper.bit $root/build/accelerator.bit
write_hw_platform -fixed -include_bit -force -file $root/build/accelerator.xsa
puts "BUILD_PASS: $root/build/accelerator.bit"
exit
