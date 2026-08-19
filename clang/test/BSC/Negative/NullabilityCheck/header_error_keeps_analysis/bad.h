// The declaration below is intentionally invalid to reproduce a broken header.
int this_is_not_valid_declaration(; // expected-error {{expected parameter declarator}} expected-error {{expected ')'}} expected-note {{to match this '('}}
