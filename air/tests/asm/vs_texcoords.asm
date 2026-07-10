; four texcoord varyings — pairs with ps_texcoords
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_texcoord0 o2
dcl_texcoord1 o3
dcl_texcoord2 o4
dcl_texcoord3 o5
mov o0, v0
mov o2, v1
mul o3, v1, c0
add o4, v1, c1
mov o5, v1.wzyx
