; PS ALU mix
ps_3_0
dcl_color0 v0
add r0, v0, c0
mul r1, r0, c1
mad r2, r1, c2, v0
sub r3, r2, c3
max r3, r3, c4
mov oC0, r3
