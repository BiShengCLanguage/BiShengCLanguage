def AsyncFunction : SubsetSubject<Function,
                             [{S->isAsyncSpecified()}], "async functions">;

def Operator : InheritableAttr {
  let Spellings = [GNU<"operator">];
  let Subjects = SubjectList<[Function]>;
  let Args = [IntArgument<"OperatorKind">];
  let Documentation = [Undocumented];
}

/// Not InheritableAttr: a redeclaration may switch mode, so it must not merge.
def SafeZone : Attr {
  let Spellings = [Keyword<"_Safe">, Keyword<"_Unsafe">];
  let Accessors = [Accessor<"isSafe", [Keyword<"_Safe">]>];
  let Subjects = SubjectList<[Function, Stmt]>;
  let SemaHandler = 0;
  let Documentation = [Undocumented];
  let AdditionalMembers = [{
    static SafeZoneSpecifier getSafeZoneSpec(const Stmt *S);
  }];
}

def EnsureInit : InheritableParamAttr {
  let Spellings = [GNU<"ensure_init">];
  let Subjects = SubjectList<[ParmVar], ErrorDiag>;
  let Documentation = [Undocumented];
}

def EnsureInitIfRet : InheritableParamAttr {
  let Spellings = [GNU<"ensure_init_if_ret">];
  let Subjects = SubjectList<[ParmVar], ErrorDiag>;
  let Args = [IntArgument<"CondValue">];
  let Documentation = [Undocumented];
}
