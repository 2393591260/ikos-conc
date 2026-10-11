; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 32))
(declare-fun a () (_ BitVec 32))
(declare-fun byte_b () (_ BitVec 32))
(declare-fun byte_a () (_ BitVec 32))
(assert
 (let ((?x19 (bvadd b (_ bv0 32))))
 (let ((?x56 (bvurem ?x19 (_ bv4 32))))
 (let ((?x21 (bvurem a (_ bv4 32))))
 (= ?x21 ?x56)))))
(assert
 (let (($x34 (bvult byte_b (_ bv4 32))))
 (let (($x9 (bvult byte_a (_ bv1 32))))
 (and $x9 $x34))))
(assert
 (let ((?x12 (bvadd b byte_b)))
 (let ((?x10 (bvadd a byte_a)))
 (= ?x10 ?x12))))
(assert
 (and (distinct a b) true))
(check-sat)
