/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/**
 * @test
 * @summary Test virtual threads using synchronized
 * @library /test/lib
 * @modules java.base/java.lang:+open
 * @run junit/othervm/timeout=10 -Xint MonitorsTest
 * @run junit/othervm/timeout=50 -Xcomp MonitorsTest
 * @run junit/othervm/timeout=50 MonitorsTest
 * @run junit/othervm/timeout=50 -XX:+FullGCALot -XX:FullGCALotInterval=1000 MonitorsTest
 */

import java.time.Duration;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.*;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class MonitorsTest {
    final int CARRIER_COUNT = 8;
    ExecutorService scheduler = Executors.newFixedThreadPool(CARRIER_COUNT);

    static AtomicInteger workerCount = new AtomicInteger(0);
    static final Object globalLock = new Object();
    static volatile boolean finish = false;
    static volatile int counter = 0;

    ///// BASIC TESTS ///////

    static final Runnable FOO = () -> {
        Object lock = new Object();
        synchronized(lock) {
            while(!finish) {
                Thread.yield();
            }
        }
        System.out.println("Exiting FOO from thread " + Thread.currentThread().getName());
    };

    static final Runnable BAR = () -> {
        synchronized(globalLock) {
            workerCount.getAndIncrement();
        }
        System.out.println("Exiting BAR from thread " + Thread.currentThread().getName());
    };

    /**
     *  Test yield while holding monitor.
     */
    //@Test
    void testBasic() throws Exception {
        final int VT_COUNT = CARRIER_COUNT;

        // Create first batch of VT threads.
        Thread firstBatch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            firstBatch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("FirstBatchVT-" + i).start(FOO);
        }

        // Give time for all threads to reach Thread.yield
        Thread.sleep(1000);

        // Create second batch of VT threads.
        Thread secondBatch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            secondBatch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("SecondBatchVT-" + i).start(BAR);
        }

        while(workerCount.get() != VT_COUNT) {}

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            firstBatch[i].join();
        }
        for (int i = 0; i < VT_COUNT; i++) {
            secondBatch[i].join();
        }
    }

    static final Runnable BAR2 = () -> {
        synchronized(globalLock) {
            counter++;
        }
        recursive2(10);
        System.out.println("Exiting BAR2 from thread " + Thread.currentThread().getName() + "with counter=" + counter);
    };

    static void recursive2(int count) {
        synchronized(Thread.currentThread()) {
            if (count > 0) {
                recursive2(count - 1);
            } else {
                synchronized(globalLock) {
                    counter++;
                    Thread.yield();
                }
            }
        }
    }

    /**
     *  Test yield while holding monitor with recursive locking.
     */
    //@Test
    void testRecursive() throws Exception {
        final int VT_COUNT = CARRIER_COUNT;
        workerCount.getAndSet(0);
        finish = false;

        // Create first batch of VT threads.
        Thread firstBatch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            firstBatch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("FirstBatchVT-" + i).start(FOO);
        }

        // Give time for all threads to reach Thread.yield
        Thread.sleep(1000);

        // Create second batch of VT threads.
        Thread secondBatch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            secondBatch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("SecondBatchVT-" + i).start(BAR2);
        }

        while(workerCount.get() != 2*VT_COUNT) {}

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            firstBatch[i].join();
        }
        for (int i = 0; i < VT_COUNT; i++) {
            secondBatch[i].join();
        }
    }

    static final Runnable FOO3 = () -> {
        synchronized(globalLock) {
            while(!finish) {
                Thread.yield();
            }
        }
        System.out.println("Exiting FOO3 from thread " + Thread.currentThread().getName());
    };

    /**
     *  Test contention on monitorenter.
     */
    //@Test
    void testContention() throws Exception {
        final int VT_COUNT = CARRIER_COUNT * 8;
        counter = 0;
        finish = false;

        // Create batch of VT threads.
        Thread batch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            batch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("BatchVT-" + i).start(FOO3);
        }

        // Give time for all threads to reach synchronized(globalLock)
        Thread.sleep(2000);

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            batch[i].join();
        }
    }

    ///// MAIN TESTS ///////

    static final int MONITORS_CNT = 12;
    static Object[] globalLockArray;

    static void recursive4_1(int lockNumber, int lockNumberOrig) {
        if (lockNumber > 0) {
            recursive4_1(lockNumber - 1, lockNumberOrig);
        } else {
            if ( lockNumberOrig % 2 == 0) {
                Thread.yield();
            }
            recursive4_2(lockNumberOrig);
        }
    }

    static void recursive4_2(int lockNumber) {
        if (lockNumber + 2 <= MONITORS_CNT - 1) {
            lockNumber += 2;
            synchronized(globalLockArray[lockNumber]) {
                System.out.println("Thread " + Thread.currentThread().getName() + " grabbed monitor " + lockNumber);
                Thread.yield();
                recursive4_2(lockNumber);
            }
        }
    }

    static final Runnable FOO4 = () -> {
        while (!finish) {
            int lockNumber = ThreadLocalRandom.current().nextInt(0, MONITORS_CNT - 1);
            synchronized(globalLockArray[lockNumber]) {
                System.out.println(Thread.currentThread().getName() + "grabbed monitor " + lockNumber);
                recursive4_1(lockNumber, lockNumber);
            }
        }
        workerCount.getAndIncrement();
        System.out.println("Exiting FOO4 from thread " + Thread.currentThread().getName());
    };

    /**
     *  Test contention on monitorenter with extra monitors on stack shared by all threads.
     */
    @Test
    void testContentionMultipleMonitors() throws Exception {
        final int VT_COUNT = CARRIER_COUNT * 8;
        workerCount.getAndSet(0);
        finish = false;

        globalLockArray = new Object[MONITORS_CNT];
        for (int i = 0; i < MONITORS_CNT; i++) {
            globalLockArray[i] = new Object();
        }

        Thread batch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            batch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("BatchVT-" + i).start(FOO4);
        }

        Thread.sleep(10000);

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            batch[i].join();
        }

        if (workerCount.get() != VT_COUNT) {
            throw new RuntimeException("testContentionMultipleMonitors2 failed. Expected " + VT_COUNT + "but found " + workerCount.get());
        }
    }


    static void recursive5_1(int lockNumber, int lockNumberOrig, Object[] myLockArray) {
        if (lockNumber > 0) {
            recursive5_1(lockNumber - 1, lockNumberOrig, myLockArray);
        } else {
            if (Math.random() < 0.5) {
                Thread.yield();
            }
            recursive5_2(lockNumberOrig, myLockArray);
        }
    }

    static void recursive5_2(int lockNumber, Object[] myLockArray) {
        if (lockNumber + 2 <= MONITORS_CNT - 1) {
            lockNumber += 2;
            synchronized (myLockArray[lockNumber]) {
                if (Math.random() < 0.5) {
                    Thread.yield();
                }
                synchronized (globalLockArray[lockNumber]) {
                    System.out.println("Thread " + Thread.currentThread().getName() + " grabbed monitor " + lockNumber);
                    Thread.yield();
                    recursive5_2(lockNumber, myLockArray);
                }
            }
        }
    }

    static final Runnable FOO5 = () -> {
        Object[] myLockArray = new Object[MONITORS_CNT];
        for (int i = 0; i < MONITORS_CNT; i++) {
            myLockArray[i] = new Object();
        }

        while (!finish) {
            int lockNumber = ThreadLocalRandom.current().nextInt(0, MONITORS_CNT - 1);
            synchronized (myLockArray[lockNumber]) {
                synchronized (globalLockArray[lockNumber]) {
                    System.out.println(Thread.currentThread().getName() + "grabbed monitor " + lockNumber);
                    recursive5_1(lockNumber, lockNumber, myLockArray);
                }
            }
        }
        workerCount.getAndIncrement();
        System.out.println("Exiting FOO5 from thread " + Thread.currentThread().getName());
    };

    /**
     *  Test contention on monitorenter with extra monitors on stack both local only and shared by all threads.
     */
    @Test
    void testContentionMultipleMonitors2() throws Exception {
        final int VT_COUNT = CARRIER_COUNT * 8;
        workerCount.getAndSet(0);
        finish = false;

        globalLockArray = new Object[MONITORS_CNT];
        for (int i = 0; i < MONITORS_CNT; i++) {
            globalLockArray[i] = new Object();
        }

        // Create batch of VT threads.
        Thread batch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            //Thread.ofVirtual().name("FirstBatchVT-" + i).start(FOO);
            batch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("BatchVT-" + i).start(FOO5);
        }

        Thread.sleep(10000);

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            batch[i].join();
        }

        if (workerCount.get() != VT_COUNT) {
            throw new RuntimeException("testContentionMultipleMonitors2 failed. Expected " + VT_COUNT + "but found " + workerCount.get());
        }
    }

    static synchronized void recursive6(int lockNumber, Object myLock) {
        if (lockNumber > 0) {
            recursive6(lockNumber - 1, myLock);
        } else {
            if (Math.random() < 0.5) {
                Thread.yield();
            } else {
                synchronized (myLock) {
                    Thread.yield();
                }
            }
        }
    }

    static final Runnable FOO6 = () -> {
        Object myLock = new Object();

        while (!finish) {
            int lockNumber = ThreadLocalRandom.current().nextInt(0, MONITORS_CNT - 1);
            synchronized (myLock) {
                synchronized (globalLockArray[lockNumber]) {
                    System.out.println(Thread.currentThread().getName() + "grabbed monitor " + lockNumber);
                    recursive6(lockNumber, myLock);
                }
            }
        }
        workerCount.getAndIncrement();
        System.out.println("Exiting FOO5 from thread " + Thread.currentThread().getName());
    };

    /**
     *  Test contention on monitorenter with synchronized methods.
     */
    @Test
    void testContentionMultipleMonitors3() throws Exception {
        final int VT_COUNT = CARRIER_COUNT * 8;
        workerCount.getAndSet(0);
        finish = false;


        globalLockArray = new Object[MONITORS_CNT];
        for (int i = 0; i < MONITORS_CNT; i++) {
            globalLockArray[i] = new Object();
        }

        // Create batch of VT threads.
        Thread batch[] = new Thread[VT_COUNT];
        for (int i = 0; i < VT_COUNT; i++) {
            batch[i] = ThreadBuilders.virtualThreadBuilder(scheduler).name("BatchVT-" + i).start(FOO6);
        }

        Thread.sleep(10000);

        finish = true;

        for (int i = 0; i < VT_COUNT; i++) {
            batch[i].join();
        }

        if (workerCount.get() != VT_COUNT) {
            throw new RuntimeException("testContentionMultipleMonitors2 failed. Expected " + VT_COUNT + "but found " + workerCount.get());
        }
    }
}
