public class compile_error_test {
    public static void foo() {
        int x = "hello";
    }
    public static void bar() {
        int y = "world";
    }
    public static void main(String[] args) {
        System.out.println("Hello from Java");
        foo();
        bar();
    }
}
