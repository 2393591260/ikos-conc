; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 32))
(declare-fun a () (_ BitVec 32))
(declare-fun byte_b () (_ BitVec 32))
(declare-fun byte_a () (_ BitVec 32))
(assert
 (let ((?x11 (bvadd b (_ bv0 32))))
 (let ((?x12 (bvurem ?x11 (_ bv4 32))))
 (let ((?x13 (bvurem a (_ bv4 32))))
 (= ?x13 ?x12)))))
(assert
 (let (($x46 (bvult byte_b (_ bv4 32))))
 (let (($x45 (bvult byte_a (_ bv4 32))))
 (and $x45 $x46))))
(assert
 (let ((?x52 (bvadd b byte_b)))
 (let ((?x53 (bvadd a byte_a)))
 (= ?x53 ?x52))))
(assert
 (and (distinct a b) true))
(check-sat)
