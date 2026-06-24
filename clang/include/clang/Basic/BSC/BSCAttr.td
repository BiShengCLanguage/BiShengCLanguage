def AsyncFunction : SubsetSubject<Function,
                             [{S->isAsyncSpecified()}], "async functions">;

def Operator : InheritableAttr {
  let Spellings = [GNU<"operator">];
  let Subjects = SubjectList<[Function]>;
  let Args = [IntArgument<"OperatorKind">];
  let Documentation = [Undocumented];
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
