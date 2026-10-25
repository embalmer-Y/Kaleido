local PS=1024
local IR=2048
for i=1,300 do
  local ps=system.heap.get_info(PS)
  local ir=system.heap.get_info(IR)
  print(string.format("V32MEM i=%d ps_free=%d ps_min=%d ps_big=%d ir_free=%d ir_min=%d ir_big=%d",i,ps.free_size,ps.minimum_free_size,ps.largest_free_block,ir.free_size,ir.minimum_free_size,ir.largest_free_block))
  delay.delay_ms(1000)
end
print("V32MEM_DONE")
