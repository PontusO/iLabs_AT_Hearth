# Security policy

Hearth is the firmware, the `AT+MT` host contract, the `iLabs_Hearth` Arduino
library and the tooling that together make a Matter device out of a
co-processor and a Raspberry Pi RP2350 host. This policy covers all of them.

## Reporting a vulnerability

Report it privately, through GitHub: open the **Security** tab of this
repository and choose **Report a vulnerability**. Please do not open a public
issue, pull request or discussion for a suspected vulnerability.

A useful report says which version and image (`AT+CGMR` answers the firmware
version; the library version is in `library.properties`), what an attacker
needs (network access, a commissioned fabric, the AT link, physical access),
what they gain, and how to reproduce it.

## What happens next

- **Within 5 working days:** we acknowledge the report.
- **Within 30 days:** we tell you our assessment: whether we consider it a
  vulnerability, its severity, and which versions it affects.
- **Within 90 days of the report:** we publish a fix or a mitigation, and a
  GitHub Security Advisory, coordinated with you. If a fix needs longer we
  tell you why and agree a new date with you before the 90 days run out.

We credit reporters in the advisory unless you ask us not to.

## Supported versions

| Version | Security fixes |
|---|---|
| 1.3.x | yes |
| older | no: update to the latest release |

## Scope

In scope: the firmware images built from this repository and the images the
library bundles, the `AT+MT` contract, the `iLabs_Hearth` library and its
`fw/` tools, and the firmware-over-the-air bundle format and its signature
check.

Out of scope, with the reason:
- The **development credentials** (vendor 0xFFF1, the SDK test DAC and
  passcode) and the **development signing keys** committed to the
  repositories. They are public on purpose and protect nothing; a product
  provisions its own.
- Vulnerabilities in the SDKs Hearth builds on (ESP-IDF, esp-matter,
  connectedhomeip, nRF Connect SDK, Silicon Labs SDK, arduino-pico): report
  them upstream. Tell us as well if Hearth ships an affected version.

## Safe harbour

We will not pursue legal action against research done in good faith that
respects this policy: that tests only devices you own or may test, does not
access or alter other people's data, does not degrade service for others, and
gives us the time above before public disclosure.
