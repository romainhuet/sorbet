# typed: strong

# This file is not valid Ruby since we reference A before
# definition. However, it would work with our autoloader, and also
# writing definitions out-of-order simulates the definitions being
# located in multiple files that are processed out of order by
# ruby-typer.
A::AB::BC

module A
  AB = B

  AV = T.let(1, Integer)
end

module B
  BC = C
end

module C
end

module F
  include E

  # Assert that this resolves to the right thing. Previously we would
  # recognize that we were hitting an ordering problem, and stub to
  # `untyped`.
  T.assert_type!(EA::AV, Integer)
end

module E
  include D
  EA = DA
end

module D
  DA = A
end
