public class debug_test {
    public static int factorial(int n) {
        if (n <= 1) return 1;
        return n * factorial(n - 1);
    }
    public static void main(String[] args) {    
        int x = 5;
        int result = factorial(x);
        System.out.println("factorial(" + x + ") = " + result);
    }
}
