[result]
AGGREGATE ([GLOBAL] aggregate [{48: sum=sum(48: sum)}] group by [[]] having [null]
    EXCHANGE GATHER
        AGGREGATE ([LOCAL] aggregate [{48: sum=sum(6: L_EXTENDEDPRICE)}] group by [[]] having [null]
            PREDICATE 5: L_QUANTITY < multiply(0.2, 50: avg)
                ANALYTIC ({50: avg=avg(5: L_QUANTITY)} [18: P_PARTKEY] [] )
                    TOP-N (order by [[18: P_PARTKEY ASC NULLS FIRST]])
                        INNER JOIN (join-predicate [2: L_PARTKEY = 18: P_PARTKEY] post-join-predicate [null])
                            EXCHANGE SHUFFLE[2]
                                SCAN (columns[2: L_PARTKEY, 5: L_QUANTITY, 6: L_EXTENDEDPRICE] predicate[null])
                            EXCHANGE SHUFFLE[18]
                                SCAN (columns[18: P_PARTKEY, 21: P_BRAND, 24: P_CONTAINER] predicate[21: P_BRAND = Brand#35 AND 24: P_CONTAINER = JUMBO CASE])
[end]

