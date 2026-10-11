; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 4))
(declare-fun a () (_ BitVec 4))
(declare-fun byte_b () (_ BitVec 4))
(declare-fun byte_a () (_ BitVec 4))
(assert
 (let ((?x36 (bvadd b (_ bv0 4))))
 (let ((?x18 (bvurem ?x36 (_ bv3 4))))
 (let ((?x22 (bvurem a (_ bv3 4))))
 (= ?x22 ?x18)))))
(assert
 (let (($x26 (bvult byte_b (_ bv3 4))))
 (let (($x39 (bvult byte_a (_ bv3 4))))
 (and $x39 $x26))))
(assert
 (let ((?x42 (bvadd b byte_b)))
 (let ((?x7 (bvadd a byte_a)))
 (= ?x7 ?x42))))
(assert
 (and (distinct a b) true))
(check-sat)
