; var_ab_vs, a vertex function writing two user varyings, a then b, for the by-identity link tests (link_test.cpp,
; compiler_test.cpp; .kiro/specs/metal-var1-linkage-crash-fix):
;
;     struct VaryAB { float4 position [[position]]; float4 a; float4 b; };
;     vertex VaryAB var_ab_vs(uint vid [[vertex_id]])
;     {
;       VaryAB v;
;       v.position = <tri_flat_vs's six positions>;
;       v.a = float4(float(vid), 0, 0, 1);
;       v.b = float4(0, float(vid), 1, 0);
;       return v;
;     }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. The front end numbers the varyings in declaration order:
; a = VAR0 ("generated(1aDv4_f)"), b = VAR1 ("generated(1bDv4_f)"). Paired with var_ba_fs (reads b, a) or var_b_fs
; (reads b alone), a link by slot routes the wrong varying; a link by identity does not.
;
;   llvm-as var_ab_vs.ll -o var_ab_vs.bc
;   ir3_compiler --chip-id 0x06030500 var_ab_vs.bc

source_filename = "var_ab_vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float>, <4 x float> }> @var_ab_vs(i32 %vid) local_unnamed_addr {
  %c4 = icmp eq i32 %vid, 4
  %p4 = select i1 %c4, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
  %c3 = icmp eq i32 %vid, 3
  %p3 = select i1 %c3, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p4
  %c2 = icmp eq i32 %vid, 2
  %p2 = select i1 %c2, <4 x float> <float -1.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p3
  %c1 = icmp eq i32 %vid, 1
  %p1 = select i1 %c1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p2
  %c0 = icmp eq i32 %vid, 0
  %p0 = select i1 %c0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float> %p1
  %f = uitofp i32 %vid to float
  %a = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %f, i32 0
  %b = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00>, float %f, i32 1
  %r0 = insertvalue <{ <4 x float>, <4 x float>, <4 x float> }> undef, <4 x float> %p0, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float>, <4 x float> }> %r0, <4 x float> %a, 1
  %r2 = insertvalue <{ <4 x float>, <4 x float>, <4 x float> }> %r1, <4 x float> %b, 2
  ret <{ <4 x float>, <4 x float>, <4 x float> }> %r2
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @var_ab_vs, !3, !7}
!3 = !{!4, !5, !6}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!"air.vertex_output", !"generated(1aDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"a"}
!6 = !{!"air.vertex_output", !"generated(1bDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"b"}
!7 = !{!8}
!8 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
