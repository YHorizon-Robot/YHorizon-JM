# Disclaimer and safety

[中文](SECURITY.md) | [English](SECURITY.en.md)

Mechanical designs, electronics, firmware, SDK, tools, and documentation in this repository (the "Project") are provided under the open-source licenses in [`LICENSE`](LICENSE). Publishing this material does not mean the authors or contributors offer a product, support, training, or any warranty.

## Disclaimer

- The Project is provided "as is". It is not warranted for any particular purpose, continuous operation, freedom from defects, or compliance with functional safety, electrical safety, or other regulations.
- A joint motor can produce enough torque in a short time to crush a hand, throw a fixture, or damage a gearbox. Wrong wiring, wrong parameters, missing calibration, or failed protection can cause personal injury and property damage.
- Anyone who machines, assembles, flashes, tests, modifies, or integrates the Project does so at their own risk. Authors and contributors are not liable for direct, indirect, incidental, or consequential damages, including but not limited to personal injury, equipment damage, data loss, downtime, and commercial loss.
- The Project is not a certified industrial product. Before using it in a humanoid, cobot, near-person setting, vehicle, or any system that could cause harm, the user must complete their own risk assessment, testing, and safety measures.
- The open-source licenses allow commercial use when the GPLv3 and CERN-OHL-S obligations are met. This file describes risk and disclaimer only. It adds no further restriction to those licenses. For closed-source integration, or to be released from the open-source obligations, contact WeChat **Yet_Latitude** for a commercial license. The template is [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md). The open-source version is designed for machining. It works, but optimal performance and long-term stability are not guaranteed. Contact us for the production version.

Using the Project means you have read the risks above. License terms are in [`LICENSE`](LICENSE). This file does not modify the open-source licenses.

## Before use

- Confirm mechanical limits, coupling/flange screws, and cable strain relief.
- Provide a hardware e-stop. Do not rely on a software stop alone.
- Jog first at low bus voltage and low current limits.
- Do not enable the position loop or high-gain impedance/force control before encoder calibration.
- Do not run at full power until current, temperature, and position limits have been verified.

## During operation

- Keep people, cables, and fixtures outside the output-flange envelope.
- Watch driver temperature, bus voltage, and fault codes.
- Power off immediately on runaway, abnormal noise, encoder jumps, or signs of over-current.
- Do not connect an uncalibrated torque loop to a humanoid or cobot load.

## Reporting safety issues

Treat firmware bugs, hardware design errors, and protection failures as safety issues. Do not post reproduction steps in a public Issue if they can directly cause over-current, runaway, or uncontrolled torque.

Report privately to maintainers when possible, including:

- Affected module (mechanical / hardware / firmware / SDK)
- Symptom and potential harm
- Software or hardware version
- Mitigations already applied

Maintainers may fix the issue or add documentation. That does not change this disclaimer: the Project still does not promise timely fixes and does not accept liability for use.
