PROGRAM()

YQL_ABI_VERSION(
    2
    27
    0
)

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/json
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql/pg_dummy
    yql/essentials/types/binary_json
)

END()
