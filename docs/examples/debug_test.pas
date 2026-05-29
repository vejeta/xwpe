program DebugTest;

function Factorial(n: integer): integer;
begin
    if n <= 1 then
        Factorial := 1
    else
        Factorial := n * Factorial(n - 1);
end;

var
    x, result: integer;
begin
    x := 5;
    result := Factorial(x);
    writeln('factorial(', x, ') = ', result);
end.
