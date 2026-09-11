def err_unsafe_action : Error<"%0 is forbidden in the safe zone">,
  BSCDiag<"SZONE", 1>;
def err_safe_zone_decl : Error<"%select{'_Safe' or '_Unsafe'|'_Safe'|'_Unsafe'}0 can only be applied to a function, function pointer, statement, or parenthesized expression">,
  BSCDiag<"SZONE", 2>;
def err_union_member_access_in_safe_zone : Error<"access to a union field is _Unsafe and requires an _Unsafe block">,
  BSCDiag<"SZONE", 3>;
def err_safe_zone_case_in_nested_braces : Error<
  "'case' label inside a nested '{ }' in the safe zone">,
  BSCDiag<"SZONE", 4,
          "`note_safe_zone_case_in_nested_braces` — remove the nested `{ }`, or wrap the switch in `_Unsafe { ... }`">;
def note_safe_zone_case_in_nested_braces : Note<
  "remove the nested '{ }', or wrap the switch in '_Unsafe { ... }'">;