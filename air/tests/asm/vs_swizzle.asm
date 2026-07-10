; swizzle stress: full reverse, replicate, mixed patterns
vs_3_0
dcl_position v0
dcl_color0 v1
dcl_position o0
dcl_color0 o1
mov r0, v1.wzyx
add r0, r0, v1.yyyy
mul r0, r0, c5.xxzz
mad r0, v1.xzxz, c6.wyxw, r0
mov o0, v0
mov o1, r0.yxwz
