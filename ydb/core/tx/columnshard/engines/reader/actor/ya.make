LIBRARY()

SRCS(
    actor.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/kqp/compute_actor
    ydb/library/actors/wilson
    ydb/library/wilson_ids
    yql/essentials/core/issue
)

END()
