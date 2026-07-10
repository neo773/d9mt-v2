; if_comp / else / endif
ps_3_0
dcl_color0 v0
if_gt v0.x, c0.x
mul r0, v0, c1
else
add r0, v0, c2
endif
mov oC0, r0
