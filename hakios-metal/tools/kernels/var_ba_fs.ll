; var_ba_fs, a fragment function reading two user varyings in the other order, b then a, for the by-identity link
; tests (link_test.cpp, compiler_test.cpp; .kiro/specs/metal-var1-linkage-crash-fix):
;
;     struct VaryBA { float4 position [[position]]; float4 b; float4 a; };
;     fragment float4 var_ba_fs(VaryBA in [[stage_in]]) { return in.b + in.a; }
;
; Hand-written, ours, MIT: nothing in it comes from Apple. The front end numbers the inputs in declaration order:
; b = VAR0 ("generated(1bDv4_f)"), a = VAR1 ("generated(1aDv4_f)"). With var_ab_vs (a = VAR0, b = VAR1) a link by slot
; swaps the two: the reorder case.
;
;   llvm-as var_ba_fs.ll -o var_ba_fs.bc
;   ir3_compiler --chip-id 0x06030500 var_ba_fs.bc

source_filename = "var_ba_fs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @var_ba_fs(<4 x float> %b, <4 x float> %a) local_unnamed_addr {
  %s = fadd <4 x float> %b, %a
  ret <4 x float> %s
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @var_ba_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6, !7}
!6 = !{i32 0, !"air.fragment_input", !"generated(1bDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"b"}
!7 = !{i32 1, !"air.fragment_input", !"generated(1aDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"a"}
