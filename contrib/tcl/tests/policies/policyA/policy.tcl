proc policyA_PolicyInit {slave {version {}}} {
    interp alias $slave tada {} tada $slave
}
proc tada {slave} {}

