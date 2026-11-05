# ShengUfs

SM8550 UFS transport and sheng persistent storage.

Current state: the native UFS root works on sheng. The controller and SMMU
defaults remain sheng's stream `0x60`; a board using the generic
`qcom,ufshc` binding can select one or two streams with
`native-ufs-stream=<id>` and `native-ufs-stream2=<id>`. Probe 7 surveys both
SIDs without writing. SM8250 alioth uses `0xe0` and `0x4e0`; probe 7 found
both exact mappings on exclusive bypass contexts, so the driver validates and
reuses them. Dual-stream takeover remains a fallback for unsafe mappings.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-ufs). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
