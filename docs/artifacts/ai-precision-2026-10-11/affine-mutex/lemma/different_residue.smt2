; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 32))
(declare-fun a () (_ BitVec 32))
(declare-fun byte_b () (_ BitVec 32))
(declare-fun byte_a () (_ BitVec 32))
(assert
 (let ((?x12 (bvadd b (_ bv1 32))))
 (let ((?x13 (bvurem ?x12 (_ bv4 32))))
 (let ((?x35 (bvurem a (_ bv4 32))))
 (= ?x35 ?x13)))))
(assert
 (let (($x37 (bvult byte_b (_ bv4 32))))
 (let (($x44 (bvult byte_a (_ bv4 32))))
 (and $x44 $x37))))
(assert
 (let ((?x5 (bvadd b byte_b)))
 (let ((?x31 (bvadd a byte_a)))
 (= ?x31 ?x5))))
(assert
 (and (distinct a b) true))
(check-sat)
