def TraitType : TypeNode<TagType>, LeafType;
def InjectedTraitNameType : TypeNode<Type>, AlwaysDependent, LeafType;
def ConditionalType : TypeNode<Type>, NeverCanonicalUnlessDependent;
def BSCQualifiedType : TypeNode<Type>, NeverCanonicalUnlessDependent;
