# Security policy

This is a developer tool that runs on your own machine against firmware you
supply. It has no server component and handles no accounts or credentials, so
the realistic security surface is small:

* the host tools that parse files you give them (update files, SD/USB images,
  captured link traffic, run directories);
* the QEMU device models and the patched Blackfin simulator, which parse
  guest-controlled data;
* the build scripts, which download and patch upstream source.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Use GitHub's
private reporting instead: the **Security** tab of this repository, then
**Report a vulnerability**.

Include what you ran, what you expected and what happened. A minimal input file
helps a lot -- but see below before attaching anything.

You can expect an answer within about a week. This is a volunteer project, so
there is no fixed fix timeline, but a confirmed problem gets a fix or a written
mitigation and, if you want it, credit.

## Firmware is not a security report

Never attach firmware, update files (`*.UPD`, `*.LDR`), flash dumps or anything
extracted from them to an issue, a pull request or a security report. They are
the manufacturer's copyrighted software and this repository does not
distribute them. Describe the input instead (size, SHA-256, which region), and
say which release it came from.
