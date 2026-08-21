def rewrite_bsc : Flag<["-"], "rewrite-bsc">, Flags<[CC1Option]>, Group<Action_Group>,
  HelpText<"Rewrite BSC source to C">;
def rewrite_bsc_line : Flag<["-"], "line">, Flags<[CC1Option]>,
  HelpText<"Insert line info when rewrite BSC">;
def dump_bscir : Flag<["-"], "dump-bscir">, Flags<[CC1Option]>,
  HelpText<"Dump BSCIR intermediate representation for debugging">,
  MarshallingInfoFlag<LangOpts<"DumpBSCIR">>;
def nullability_check : Joined<["-"], "nullability-check=">, Flags<[CC1Option]>,
  HelpText<"Select Nullability Check Enable Zone">,
  Values<"safeonly,all">, NormalizedValuesScope<"LangOptions">,
  NormalizedValues<["NC_SAFE", "NC_ALL"]>,
  MarshallingInfoEnum<LangOpts<"NullabilityCheck">, "NC_SAFE">;
def uninit_check : Joined<["-"], "uninit-check=">, Flags<[CC1Option]>,
  HelpText<"Select uninitialized variable check mode">,
  Values<"safeonly,none,all">, NormalizedValuesScope<"LangOptions">,
  NormalizedValues<["UC_SAFE", "UC_NONE", "UC_ALL"]>,
  MarshallingInfoEnum<LangOpts<"UninitCheck">, "UC_SAFE">;
defm bsc_experimental : BoolFOption<"bsc-experimental",
  LangOpts<"BSCExperimental">, DefaultFalse,
  PosFlag<SetTrue, [CC1Option], "Enable the BSC experimental features">,
  NegFlag<SetFalse>>;
