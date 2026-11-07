# Security policy

## Supported versions

Security fixes are made on the `main` branch and in the latest
release. Older releases are not updated.

## Reporting a vulnerability

Please don't report security problems in public issues.

Report them privately through GitHub instead: open this repository's
**Security** tab and choose **Report a vulnerability**. Only the
repository's maintainers can see the report.

Please include:

- the cputty version (`plink -V`, or the About box on Windows) and
  your operating system;
- what an attacker could do, and what they need to do it (for example,
  control of the SSH server you connect to, or access to your
  machine);
- steps to reproduce the problem, or a proof of concept.

## Problems in upstream PuTTY

cputty is a fork of PuTTY. If the problem is in code that cputty hasn't
changed, official PuTTY is probably affected too, so please also report
it to the PuTTY developers. Their feedback page explains how, under
"Reporting security vulnerabilities":
<https://www.chiark.greenend.org.uk/~sgtatham/putty/feedback.html>
