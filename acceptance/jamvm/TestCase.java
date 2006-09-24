/*
 * Copyright (C) 2006  Pekka Enberg
 *
 * This file is released under the GPL version 2. Please refer to the file
 * LICENSE for details.
 */
package jamvm;

/**
 * @author Pekka Enberg
 */
public class TestCase {
    protected static int retval;

    protected static void assertEquals(int expected, int actual) {
        if (expected != actual) {
            fail(/*"Expected '" + expected + "', but was '" + actual + "'."*/);
        }
    }

    protected static void assertNull(Object actual) {
        if (actual != null) {
            fail(/*"Expected null, but was '" + actual + "'."*/);
        }
    }

    protected static void assertNotNull(Object actual) {
        if (actual == null) {
            fail(/*"Expected non-null, but was '" + actual + "'."*/);
        }
    }

    protected static void fail(/*String msg*/) {
//        System.out.println(msg);
        retval = 1;
    }
}
