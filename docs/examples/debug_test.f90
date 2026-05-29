program debug_test
    implicit none
    integer :: x, result

    x = 5
    result = factorial(x)
    print *, 'factorial(', x, ') = ', result

contains
    recursive function factorial(n) result(f)
        integer, intent(in) :: n
        integer :: f
        if (n <= 1) then
            f = 1
        else
            f = n * factorial(n - 1)
        end if
    end function factorial
end program debug_test
