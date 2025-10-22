Updated RPCs
------------

- Information on soft fork status has been moved from `getblockchaininfo`
  to `getdeploymentinfo` which allows querying soft fork status at any
  block, rather than just at the chain tip. Inclusion of soft fork
  status in `getblockchaininfo` is currently available but this will be
  restricted or removed in a future release.
