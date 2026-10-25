local h=system.heap
print("V32PROBE keys: "..tostring(h.caps).." info="..tostring(h.get_info))
if h.caps then print("V32PROBE caps.SPIRAM="..tostring(h.caps.MALLOC_CAP_SPIRAM).." caps.INTERNAL="..tostring(h.caps.MALLOC_CAP_INTERNAL)) end
local p=h.get_info(8) print("V32PROBE psram(8): free="..p.free_size.." min="..p.minimum_free_size)
