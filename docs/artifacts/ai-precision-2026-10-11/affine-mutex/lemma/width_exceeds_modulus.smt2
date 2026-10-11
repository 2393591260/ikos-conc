; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 32))
(declare-fun a () (_ BitVec 32))
(declare-fun byte_b () (_ BitVec 32))
(declare-fun byte_a () (_ BitVec 32))
(assert
 (let ((?x5 (bvadd b (_ bv0 32))))
 (let ((?x147 (bvurem ?x5 (_ bv4 32))))
 (let ((?x45 (bvurem a (_ bv4 32))))
 (= ?x45 ?x147)))))
(assert
 (let (($x53 (bvult byte_b (_ bv4 32))))
 (let (($x55 (bvult byte_a (_ bv8 32))))
 (and $x55 $x53))))
(assert
 (let ((?x1221 (bvadd b byte_b)))
 (let ((?x22 (bvadd a byte_a)))
 (= ?x22 ?x1221))))
(assert
 (and (distinct a b) true))
(check-sat)
