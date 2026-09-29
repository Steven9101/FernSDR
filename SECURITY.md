# Security

Report a security problem privately, through
[GitHub's form](https://github.com/Steven9101/FernSDR/security/advisories/new),
not in a public issue. Say what an attacker can do and how, with the version
(the admin panel's Updates page shows it, or
`/opt/fernsdr/current/fernsdr --version` on the machine). The release that
fixes it says who found the problem, unless you would rather it did not.

What counts: anything that lets someone who is not the operator change the
receiver, run code on the machine or in other listeners' pages, read what
the admin panel keeps, or take the receiver down with less effort than
filling its uplink. A module or decoder from the default catalog counts too;
report it here or in its own repository.

Only the newest release is fixed. A receiver installed with `install.sh`
gets the fix from its Updates page, or by running the installer again, which
is also how a container is updated.
