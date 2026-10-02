# AnimeJaNai inference permission request

**DRAFT — not sent. No permission received.**

**To:** the copyright holder(s) of `the-database/animejanai-inference`  
**Subject:** License request for private native-player integration

I am developing an independent C++/Qt/libmpv player for private, local,
non-commercial use. I would like to adopt your original spatial and RIFE
inference implementation from:

- Repository: https://github.com/the-database/animejanai-inference
- Commit: `738ce3b3020b3950c48ce3122a08f32af531d4fa`
- Components: the source of `aji.dll`, `aji_trt.dll`, and `aji_dml.dll`

No license grant was located at that commit. Could you provide an explicit
license permitting us to copy the source into this project's own build,
modify it, compile it, and link/load/execute the resulting DLLs locally?
Required changes include TensorRT 11.3 build configuration, structured status,
prepared-only activation, content-addressed engine ownership, and acknowledged
suspend/drain behavior. The original neural implementation would be retained.

Please identify attribution, notice, corresponding-source, and other conditions,
any excluded files, and whether the grant covers all relevant copyright holders.
We can retain the exact source commit, patches, build configuration and notices;
any corresponding-source/relinking obligations would be preserved.

**Separate question for possible future distribution:** would the license also
permit redistribution of the adopted source, our patches, and built DLLs?
Distribution is not currently authorized; this is a request for clarification,
not an intention to publish before the terms permit it.

Until a grant is received, source adoption and native runtime delivery remain
blocked. No substitute inference implementation is proposed. See
`manifest/THIRD_PARTY_NOTICES.md` and `manifest/components.json` for the evidence
and affected components. Sending this request requires the project owner's
explicit approval; no recipient address or message has been invented or sent.
