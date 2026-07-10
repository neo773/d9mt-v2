; consume four texcoord varyings — pairs with vs_texcoords
ps_3_0
dcl_texcoord0 v0
dcl_texcoord1 v1
dcl_texcoord2 v2
dcl_texcoord3 v3
add r0, v0, v1
mad r0, v2, c2, r0
min r0, r0, v3
mov oC0, r0
